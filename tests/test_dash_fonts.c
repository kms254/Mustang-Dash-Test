/*
 * Invariant test: the generated EVE legacy bitmap-font header
 * (MustangDash/dash_fonts.h, from tools/make_dash_fonts.py) must honor the
 * BT81x Programming Guide 5.4.1 legacy font format -- 148-byte metric block,
 * L4 format code 17, stride = cell_w/2 with an even cell width, width bytes
 * zero below firstchar, zlib-wrapped glyph streams -- and the plan's RAM_G
 * budget. Uses the header's own _CELL_W/_STRIDE/... defines so a regenerated
 * header keeps passing; only the format rules themselves are hardcoded.
 *
 * Runs on the host:
 *   gcc -std=c11 -Wall -Wextra -Werror -I MustangDash tests/test_dash_fonts.c \
 *       -lm -o /tmp/tdf && /tmp/tdf
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define PROGMEM /* host build: no flash attribute */
#include "dash_fonts.h"

_Static_assert(DASH_FONT_COUNT == 8, "the dash uses exactly 8 font instances");

/* compile-time format rules, per instance (regeneration-proof: everything
 * comes from the header's own defines; only the rules are hardcoded) */
#define CHECK_FONT(NAME, name)                                                 \
    _Static_assert(sizeof(dash_font_##name##_metrics) == 148,                  \
                   #NAME " metric block must be 148 bytes");                   \
    _Static_assert(sizeof(dash_font_##name##_glyphs_z) ==                      \
                       DASH_FONT_##NAME##_ZBYTES,                              \
                   #NAME " _ZBYTES must match the zlib array size");           \
    _Static_assert(DASH_FONT_##NAME##_CELL_W % 2 == 0,                         \
                   #NAME " cell width must be even (L4 stride exactness)");    \
    _Static_assert(DASH_FONT_##NAME##_STRIDE ==                                \
                       DASH_FONT_##NAME##_CELL_W / 2,                          \
                   #NAME " stride must be cell_w/2 for L4");                   \
    _Static_assert(DASH_FONT_##NAME##_FIRSTCHAR < 128,                         \
                   #NAME " firstchar must be < 128 (legacy font limit)");      \
    _Static_assert(DASH_FONT_##NAME##_HANDLE >= 1 &&                           \
                       DASH_FONT_##NAME##_HANDLE <= 14,                        \
                   #NAME " handle must be in 1..14")

CHECK_FONT(HERO, hero);
CHECK_FONT(BIG, big);
CHECK_FONT(MID, mid);
CHECK_FONT(VAL, val);
CHECK_FONT(SMALL, small);
CHECK_FONT(TITLE, title);
CHECK_FONT(LABEL, label);
CHECK_FONT(TINY, tiny);

/* glyph sets per instance, mirroring tools/make_dash_fonts.py: the ASCII
 * cell codes stored (the degree-sign artwork lives in F_VAL's '*' cell and
 * the middle-dot artwork in F_LABEL/F_TINY's ';' cell, per the plan) */
#define LABEL_SET " \"/0123456789;ABCDEFGHIJKLMNOPQRSTUVWXYZ"
static const char *const FONT_NAMES[DASH_FONT_COUNT] = {
    "F_HERO", "F_BIG", "F_MID", "F_VAL", "F_SMALL", "F_TITLE", "F_LABEL", "F_TINY",
};
static const char *const FONT_SETS[DASH_FONT_COUNT] = {
    "0123456789-",
    "0123456789:.-",
    "0123456789:.-+",
    "0123456789:.-+*",
    "0123456789.",
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    LABEL_SET,
    LABEL_SET,
};

/* expected per-instance values, taken from the header's defines so the test
 * survives regeneration */
static const uint32_t EXP_GLYPH_BYTES[DASH_FONT_COUNT] = {
    DASH_FONT_HERO_GLYPH_BYTES,  DASH_FONT_BIG_GLYPH_BYTES,
    DASH_FONT_MID_GLYPH_BYTES,   DASH_FONT_VAL_GLYPH_BYTES,
    DASH_FONT_SMALL_GLYPH_BYTES, DASH_FONT_TITLE_GLYPH_BYTES,
    DASH_FONT_LABEL_GLYPH_BYTES, DASH_FONT_TINY_GLYPH_BYTES,
};
static const uint32_t EXP_ZBYTES[DASH_FONT_COUNT] = {
    DASH_FONT_HERO_ZBYTES,  DASH_FONT_BIG_ZBYTES,  DASH_FONT_MID_ZBYTES,
    DASH_FONT_VAL_ZBYTES,   DASH_FONT_SMALL_ZBYTES, DASH_FONT_TITLE_ZBYTES,
    DASH_FONT_LABEL_ZBYTES, DASH_FONT_TINY_ZBYTES,
};
static const uint16_t EXP_CELL_W[DASH_FONT_COUNT] = {
    DASH_FONT_HERO_CELL_W,  DASH_FONT_BIG_CELL_W,   DASH_FONT_MID_CELL_W,
    DASH_FONT_VAL_CELL_W,   DASH_FONT_SMALL_CELL_W, DASH_FONT_TITLE_CELL_W,
    DASH_FONT_LABEL_CELL_W, DASH_FONT_TINY_CELL_W,
};
static const uint16_t EXP_CELL_H[DASH_FONT_COUNT] = {
    DASH_FONT_HERO_CELL_H,  DASH_FONT_BIG_CELL_H,   DASH_FONT_MID_CELL_H,
    DASH_FONT_VAL_CELL_H,   DASH_FONT_SMALL_CELL_H, DASH_FONT_TITLE_CELL_H,
    DASH_FONT_LABEL_CELL_H, DASH_FONT_TINY_CELL_H,
};
static const uint8_t EXP_HANDLE[DASH_FONT_COUNT] = {
    DASH_FONT_HERO_HANDLE,  DASH_FONT_BIG_HANDLE,   DASH_FONT_MID_HANDLE,
    DASH_FONT_VAL_HANDLE,   DASH_FONT_SMALL_HANDLE, DASH_FONT_TITLE_HANDLE,
    DASH_FONT_LABEL_HANDLE, DASH_FONT_TINY_HANDLE,
};
static const uint8_t EXP_FIRSTCHAR[DASH_FONT_COUNT] = {
    DASH_FONT_HERO_FIRSTCHAR,  DASH_FONT_BIG_FIRSTCHAR,
    DASH_FONT_MID_FIRSTCHAR,   DASH_FONT_VAL_FIRSTCHAR,
    DASH_FONT_SMALL_FIRSTCHAR, DASH_FONT_TITLE_FIRSTCHAR,
    DASH_FONT_LABEL_FIRSTCHAR, DASH_FONT_TINY_FIRSTCHAR,
};

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static void expectf(int cond, const char *font, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s: %s\n", font, msg);
        failures++;
    }
}

/* little-endian u32 read from a metric block */
static uint32_t rd32(const uint8_t *m, int off)
{
    return (uint32_t)m[off] | ((uint32_t)m[off + 1] << 8) |
           ((uint32_t)m[off + 2] << 16) | ((uint32_t)m[off + 3] << 24);
}

static uint32_t align4(uint32_t n)
{
    return (n + 3U) & ~3U;
}

int main(void)
{
    uint32_t total_decoded = 0;

    for (int i = 0; i < DASH_FONT_COUNT; i++)
    {
        const DashFontDesc *d = &DASH_FONTS[i];
        const char *name = FONT_NAMES[i];
        const char *set = FONT_SETS[i];
        const uint8_t *m = d->metrics;

        /* registry entries must agree with the per-instance defines */
        expectf(d->handle == EXP_HANDLE[i], name, "registry handle must match its define");
        expectf(d->firstchar == EXP_FIRSTCHAR[i], name, "registry firstchar must match its define");
        expectf(d->cell_w == EXP_CELL_W[i], name, "registry cell_w must match its define");
        expectf(d->cell_h == EXP_CELL_H[i], name, "registry cell_h must match its define");
        expectf(d->glyph_bytes == EXP_GLYPH_BYTES[i], name, "registry glyph_bytes must match its define");
        expectf(d->zbytes == EXP_ZBYTES[i], name, "registry zbytes must match its define");

        /* metric-block fields (BT81x guide 5.4.1), all little-endian u32 */
        expectf(rd32(m, 128) == 17U, name, "format field must be 17 (L4)");
        expectf(rd32(m, 132) == d->stride, name, "stride field must match registry stride");
        expectf(rd32(m, 136) == d->cell_w, name, "cell-width field must match registry cell_w");
        expectf(rd32(m, 140) == d->cell_h, name, "cell-height field must match registry cell_h");
        expectf(rd32(m, 144) == 0U, name, "gptr must be 0 (firmware patches it at boot)");
        expectf(d->cell_w % 2 == 0, name, "cell width must be even");
        expectf(d->stride == d->cell_w / 2U, name, "stride must be cell_w/2 for L4");

        /* glyph-set geometry: firstchar/lastchar from the set, cells contiguous */
        int first = 128, last = -1;
        for (const char *p = set; *p; p++)
        {
            int c = (unsigned char)*p;
            if (c < first) { first = c; }
            if (c > last) { last = c; }
        }
        expectf(first < 128 && last < 128, name, "all stored glyph codes must be < 128");
        expectf(d->firstchar == first, name, "firstchar must be the set's minimum code");
        expectf(d->glyph_bytes ==
                    (uint32_t)d->stride * d->cell_h * (uint32_t)(last - first + 1),
                name, "_GLYPH_BYTES must be stride*cell_h*ncells");

        /* width bytes: zero outside the set, positive for every set member */
        int in_set[128];
        memset(in_set, 0, sizeof(in_set));
        for (const char *p = set; *p; p++)
        {
            in_set[(unsigned char)*p] = 1;
        }
        for (int c = 0; c < 128; c++)
        {
            if (c < first)
            {
                expectf(m[c] == 0, name, "width bytes below firstchar must be 0");
            }
            if (in_set[c])
            {
                expectf(m[c] > 0, name, "every set glyph must have a positive advance");
            }
            else
            {
                expectf(m[c] == 0, name, "codes outside the set must have width 0");
            }
        }

        /* glyph stream must be a zlib stream (EVE_cmd_inflate consumes zlib) */
        expectf(d->zbytes > 0, name, "compressed glyph stream must be non-empty");
        expectf(d->glyphs_z[0] == 0x78, name, "glyph stream must start with the zlib magic 0x78");

        total_decoded += align4(148U) + align4(d->glyph_bytes);
    }

    /* RAM_G sanity: all instances (metrics + inflated glyphs, 4-byte packed)
     * must leave room for the splash/pony bitmaps in the 1 MiB RAM_G */
    expect(total_decoded < 400000U, "total decoded font bytes must stay under 400000");

    if (failures == 0)
    {
        printf("OK: dash fonts honor the EVE legacy format "
               "(148B metrics, L4, even cells, widths, zlib; %u B decoded)\n",
               total_decoded);
        return 0;
    }
    return 1;
}
